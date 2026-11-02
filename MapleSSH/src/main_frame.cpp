#include "main_frame.hpp"
#include "dialogs.hpp"
#include <wx/artprov.h>
#include <wx/choicdlg.h>
#include <wx/splitter.h>
#include <wx/aboutdlg.h>
#include <wx/textdlg.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <map>

namespace maple {
namespace {
enum { IdNew = wxID_HIGHEST + 1, IdEdit, IdDelete, IdConnect, IdDisconnect, IdReconnect, IdSettings, IdBackup, IdRestore, IdCancel, IdLicenses };
class SessionItem final : public wxTreeItemData {
public:
    explicit SessionItem(std::string value) : id(std::move(value)) {}
    std::string id;
};
}
MainFrame::MainFrame(Storage storage, Vault vault, std::string master)
    : wxFrame(nullptr, wxID_ANY, "MapleSSH", wxDefaultPosition, wxSize(1180, 760)),
      storage_(std::move(storage)), vault_(std::move(vault)), master_(std::move(master)),
      keys_(storage_.Directory() / "known_hosts"), timer_(this) {
    SetMinSize(wxSize(850, 540));
    auto* menus = new wxMenuBar;
    auto* sessions = new wxMenu;
    sessions->Append(IdNew, wxS("新增会话\tCtrl+N")); sessions->Append(IdEdit, wxS("编辑会话")); sessions->Append(IdDelete, wxS("删除会话"));
    sessions->AppendSeparator(); sessions->Append(IdConnect, wxS("连接\tCtrl+Enter")); sessions->Append(IdDisconnect, wxS("断开当前连接")); sessions->Append(IdReconnect, wxS("重新连接"));
    sessions->AppendSeparator(); sessions->Append(wxID_EXIT, wxS("退出"));
    auto* backup = new wxMenu;
    backup->Append(IdSettings, wxS("WebDAV 设置…")); backup->AppendSeparator();
    backup->Append(IdBackup, wxS("手动备份…")); backup->Append(IdRestore, wxS("手动恢复…")); backup->Append(IdCancel, wxS("取消当前操作"));
    auto* help = new wxMenu; help->Append(wxID_ABOUT, wxS("关于 MapleSSH")); help->Append(IdLicenses, wxS("第三方许可证…"));
    menus->Append(sessions, wxS("会话")); menus->Append(backup, wxS("备份")); menus->Append(help, wxS("帮助")); SetMenuBar(menus);
    auto* toolbar = CreateToolBar(wxTB_FLAT | wxTB_TEXT);
    toolbar->AddTool(IdNew, wxS("新增"), wxArtProvider::GetBitmap(wxART_NEW, wxART_TOOLBAR));
    toolbar->AddTool(IdEdit, wxS("编辑"), wxArtProvider::GetBitmap(wxART_EDIT, wxART_TOOLBAR));
    toolbar->AddTool(IdConnect, wxS("连接"), wxArtProvider::GetBitmap(wxART_GO_FORWARD, wxART_TOOLBAR));
    toolbar->AddTool(IdDisconnect, wxS("断开"), wxArtProvider::GetBitmap(wxART_CROSS_MARK, wxART_TOOLBAR));
    toolbar->AddSeparator();
    toolbar->AddTool(IdBackup, wxS("备份"), wxArtProvider::GetBitmap(wxART_FILE_SAVE, wxART_TOOLBAR));
    toolbar->AddTool(IdRestore, wxS("恢复"), wxArtProvider::GetBitmap(wxART_UNDO, wxART_TOOLBAR));
    toolbar->AddTool(IdSettings, wxS("设置"), wxArtProvider::GetBitmap(wxART_EXECUTABLE_FILE, wxART_TOOLBAR));
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
        wxS("Windows 原生 SSH 工作台\n\n新增会话，然后双击连接。\nCtrl+Shift+C / V 复制粘贴，Ctrl+滚轮调整终端字体。\n\n配置在本机加密保存；WebDAV 只在手动操作时备份。\n主机指纹仅保存在本机，不进入备份。"));
    description->SetFont(wxFont(wxFontInfo(11))); welcome_layout->Add(description, 0, wxALIGN_CENTER);
    welcome_layout->AddStretchSpacer(); welcome->SetSizer(welcome_layout); notebook_->AddPage(welcome, wxS("开始"), true);
    splitter->SetMinimumPaneSize(180); splitter->SplitVertically(tree_, notebook_, 245);
    auto* layout = new wxBoxSizer(wxVERTICAL); layout->Add(splitter, 1, wxEXPAND); SetSizer(layout);
    CreateStatusBar(2); const int widths[]{-1, 220}; SetStatusWidths(2, widths);
    SetStatusText(wxS("就绪 · 凭据加密保存在程序同目录"), 0); SetStatusText(wxS("手动 WebDAV 备份"), 1);
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
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { if (auto* tab = ActiveTab()) { tab->client->Start(tab->terminal->Rows(), tab->terminal->Columns()); tab->terminal->SetFocus(); } }, IdReconnect);
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
        else if (event.GetId() >= IdNew && event.GetId() <= IdRestore) event.Enable(!job_);
    });
    tree_->Bind(wxEVT_TREE_ITEM_ACTIVATED, [this](wxTreeEvent&) { ConnectSelected(); });
    notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, [this](wxAuiNotebookEvent& event) {
        auto* page = notebook_->GetPage(event.GetSelection());
        const auto found = std::find_if(tabs_.begin(), tabs_.end(), [page](const auto& tab) { return tab->terminal == page; });
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
    OPENSSL_cleanse(master_.data(), master_.size());
}
void MainFrame::ShowError(const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), "MapleSSH", wxOK | wxICON_ERROR, this); }
void MainFrame::BuildTree() {
    const auto selected = SelectedId();
    tree_->DeleteAllItems(); const auto root = tree_->AddRoot(wxS("会话"));
    std::map<std::string, wxTreeItemId> groups;
    for (const auto& session : vault_.sessions) {
        const auto group = session.group.empty() ? std::string("未分组") : session.group;
        if (!groups.contains(group)) groups[group] = tree_->AppendItem(root, wxString::FromUTF8(group));
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
    if (!create) {
        const auto id = SelectedId(); const auto found = std::find_if(vault_.sessions.begin(), vault_.sessions.end(), [&id](const auto& session) { return session.id == id; });
        if (found == vault_.sessions.end()) return; initial = *found;
    }
    SessionDialog dialog(this, initial); if (dialog.ShowModal() != wxID_OK) return;
    auto updated = vault_; const auto session = dialog.Result();
    if (create) updated.sessions.push_back(session);
    else for (auto& entry : updated.sessions) if (entry.id == session.id) entry = session;
    Save(std::move(updated));
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
    for (auto& tab : tabs_) if (tab->terminal == page) return tab.get();
    return nullptr;
}
void MainFrame::ConnectSelected() {
    if (job_) return;
    try {
        const auto id = SelectedId();
        const auto found = std::find_if(vault_.sessions.begin(), vault_.sessions.end(), [&id](const auto& session) { return session.id == id; });
        if (found == vault_.sessions.end()) return;
        if (tabs_.size() >= 16) throw std::runtime_error("首版最多同时打开 16 个终端标签");
        auto tab = std::make_unique<Tab>(); tab->session = *found;
        tab->client = std::make_unique<SshClient>(*found); tab->terminal = new TerminalPanel(notebook_);
        auto* client = tab->client.get();
        tab->terminal->SetSender([this, client](std::string text) { try { client->Send(std::move(text)); } catch (const std::exception& error) { SetStatusText(wxString::FromUTF8(error.what())); } });
        tab->terminal->SetResize([client](int rows, int columns) { client->Resize(rows, columns); });
        notebook_->AddPage(tab->terminal, wxString::FromUTF8(found->name), true);
        tab->client->Start(tab->terminal->Rows(), tab->terminal->Columns()); tab->terminal->SetFocus(); tabs_.push_back(std::move(tab));
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
    Launch(job, [this, recovery_copy](Job& state) { if (recovery_copy) storage_.SaveRecoveryCopy(); storage_.Save(state.vault, master_); });
}
void MainFrame::Backup() {
    if (job_) return;
    try {
        auto job = std::make_shared<Job>(); job->kind = JobKind::Backup;
        job->client = std::make_shared<WebDavClient>(vault_.webdav); job->name = WebDavClient::NewBackupName();
        if (wxMessageBox(wxS("将会话、SSH 密码和私钥加密上传到所设置的 WebDAV。\n不包含本机指纹或 WebDAV 登录资料。确认备份？"), wxS("手动备份"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        const auto sessions = vault_.sessions; const auto password = master_;
        Launch(job, [sessions, password](Job& state) { const auto encrypted = Encrypt(SerializeBackup(sessions), password, "backup"); state.client->Upload(state.name, encrypted); });
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
    wxPasswordEntryDialog password(this, wxS("输入该备份创建时使用的主密码"), wxS("解密云端备份"));
    if (password.ShowModal() != wxID_OK) return;
    try {
        auto job = std::make_shared<Job>(); job->kind = JobKind::Download; job->name = name;
        job->client = std::make_shared<WebDavClient>(vault_.webdav); const auto secret = password.GetValue().ToStdString(wxConvUTF8);
        Launch(job, [secret](Job& state) { state.restored = ParseBackup(Decrypt(state.client->Download(state.name), secret, "backup")); });
    } catch (const std::exception& error) { ShowError(error); }
}
void MainFrame::CompleteJob() {
    if (!job_ || !job_->done.load(std::memory_order_acquire)) return;
    if (background_.joinable()) background_.join();
    auto completed = std::move(job_); SetStatusText(wxS("就绪"));
    if (!completed->error.empty()) { std::runtime_error error(completed->error); ShowError(error); return; }
    switch (completed->kind) {
        case JobKind::Save: vault_ = std::move(completed->vault); BuildTree(); SetStatusText(wxS("配置已加密保存")); break;
        case JobKind::Backup: wxMessageBox(wxS("备份已上传并通过回读校验：\n") + wxString::FromUTF8(completed->name), wxS("备份成功"), wxOK | wxICON_INFORMATION, this); break;
        case JobKind::List: {
            if (completed->names.empty()) { wxMessageBox(wxS("该目录没有 MapleSSH 备份"), wxS("手动恢复"), wxOK | wxICON_INFORMATION, this); break; }
            wxArrayString choices; for (const auto& name : completed->names) choices.Add(wxString::FromUTF8(name));
            wxSingleChoiceDialog choice(this, wxS("选择要恢复的加密备份"), wxS("手动恢复"), choices);
            if (choice.ShowModal() == wxID_OK) StartDownload(completed->names.at(choice.GetSelection()));
            break;
        }
        case JobKind::Download: {
            const auto message = wxString::Format(wxS("备份含 %zu 条会话。\n将替换当前会话列表，并先保留本机加密副本。\n本机指纹、WebDAV 设置和已打开连接保持不变。继续？"), completed->restored.size());
            if (wxMessageBox(message, wxS("确认恢复"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) break;
            auto updated = vault_; updated.sessions = std::move(completed->restored); Save(std::move(updated), true); break;
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
                const auto page = notebook_->GetPageIndex(tab->terminal);
                if (page != wxNOT_FOUND) notebook_->SetPageText(page, wxString::FromUTF8(tab->session.name + " · " + event.text));
                tab->terminal->Feed("\r\n\x1b[90m[MapleSSH] " + event.text + "\x1b[0m\r\n");
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
