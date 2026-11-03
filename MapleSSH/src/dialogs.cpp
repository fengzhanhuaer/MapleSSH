#include "dialogs.hpp"
#include "crypto.hpp"
#include "storage.hpp"
#include "webdav.hpp"
#include <wx/filedlg.h>
#include <wx/choice.h>

namespace maple {
namespace {
wxTextCtrl* Field(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const std::string& value, long style = 0) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto* control = new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(value), wxDefaultPosition, wxDefaultSize, style);
    grid->Add(control, 1, wxEXPAND);
    return control;
}
std::string Text(wxTextCtrl* control) { return control->GetValue().ToStdString(wxConvUTF8); }
}
CredentialDialog::CredentialDialog(wxWindow* parent, Credential initial)
    : wxDialog(parent, wxID_ANY, initial.id.empty() ? wxS("新增凭据") : wxS("编辑凭据"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), result_(std::move(initial)) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(this, wxID_ANY, wxS("同一份凭据可供多台主机选择；修改后在下次连接或重连时生效。")), 0, wxALL, 16);
    auto* grid = new wxFlexGridSizer(2, 8, 12); grid->AddGrowableCol(1);
    name_ = Field(this, grid, wxS("凭据名称"), result_.name);
    username_ = Field(this, grid, wxS("用户名"), result_.username);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("认证方式")), 0, wxALIGN_CENTER_VERTICAL);
    auth_ = new wxChoice(this, wxID_ANY); auth_->Append(wxS("密码")); auth_->Append(wxS("私钥"));
    auth_->SetSelection(result_.auth == AuthMethod::Password ? 0 : 1); grid->Add(auth_, 1, wxEXPAND);
    password_ = Field(this, grid, wxS("SSH 密码"), result_.password, wxTE_PASSWORD);
    passphrase_ = Field(this, grid, wxS("私钥口令"), result_.passphrase, wxTE_PASSWORD);
    layout->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
    auto* key_header = new wxBoxSizer(wxHORIZONTAL);
    key_header->Add(new wxStaticText(this, wxID_ANY, wxS("私钥内容（存入本机配置，不依赖外部文件路径）")), 1, wxALIGN_CENTER_VERTICAL);
    auto* import = new wxButton(this, wxID_ANY, wxS("导入私钥…")); key_header->Add(import);
    layout->Add(key_header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);
    private_key_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(result_.private_key), wxDefaultPosition, wxSize(560, 130), wxTE_MULTILINE | wxTE_DONTWRAP);
    private_key_->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
    layout->Add(private_key_, 1, wxEXPAND | wxALL, 16);
    layout->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);
    SetSizerAndFit(layout); SetMinSize(wxSize(640, 480));
    const auto toggle = [this] { password_->Enable(auth_->GetSelection() == 0); passphrase_->Enable(auth_->GetSelection() == 1); private_key_->Enable(auth_->GetSelection() == 1); };
    toggle(); auth_->Bind(wxEVT_CHOICE, [toggle](wxCommandEvent&) { toggle(); });
    import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog file(this, wxS("导入 SSH 私钥"), wxEmptyString, wxEmptyString, wxS("所有文件 (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (file.ShowModal() != wxID_OK) return;
        try {
            const auto bytes = ReadBytes(std::filesystem::path(file.GetPath().ToStdWstring()));
            if (bytes.size() > 1024 * 1024) throw std::runtime_error("私钥超过 1 MiB");
            const auto text = wxString::FromUTF8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (text.empty()) throw std::runtime_error("仅支持文本格式的 SSH 私钥");
            private_key_->SetValue(text); auth_->SetSelection(1); password_->Disable(); passphrase_->Enable(); private_key_->Enable();
        } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("导入失败"), wxOK | wxICON_ERROR, this); }
    });
    Bind(wxEVT_BUTTON, &CredentialDialog::Accept, this, wxID_OK);
}
void CredentialDialog::Accept(wxCommandEvent&) {
    try {
        auto updated = result_; if (updated.id.empty()) updated.id = RandomId();
        updated.name = Text(name_); updated.username = Text(username_);
        updated.auth = auth_->GetSelection() == 0 ? AuthMethod::Password : AuthMethod::PrivateKey;
        updated.password = updated.auth == AuthMethod::Password ? Text(password_) : "";
        updated.private_key = updated.auth == AuthMethod::PrivateKey ? Text(private_key_) : "";
        updated.passphrase = updated.auth == AuthMethod::PrivateKey ? Text(passphrase_) : "";
        ValidateCredentials({updated}); result_ = std::move(updated); EndModal(wxID_OK);
    } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("凭据无效"), wxOK | wxICON_WARNING, this); }
}
CredentialManagerDialog::CredentialManagerDialog(wxWindow* parent, Vault initial)
    : wxDialog(parent, wxID_ANY, wxS("凭据管理"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), vault_(std::move(initial)) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(this, wxID_ANY, wxS("凭据独立于主机保存，可被多台主机复用。正在使用的凭据不能删除。")), 0, wxALL, 16);
    list_ = new wxListBox(this, wxID_ANY, wxDefaultPosition, wxSize(550, 230));
    layout->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 16);
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* create = new wxButton(this, wxID_ANY, wxS("新增凭据…")); auto* edit = new wxButton(this, wxID_ANY, wxS("编辑…")); auto* remove = new wxButton(this, wxID_ANY, wxS("删除"));
    actions->Add(create, 0, wxRIGHT, 8); actions->Add(edit, 0, wxRIGHT, 8); actions->Add(remove);
    layout->Add(actions, 0, wxALL, 16); layout->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);
    SetSizerAndFit(layout); SetMinSize(wxSize(620, 400)); RefreshList();
    create->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Edit(true); }); edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Edit(false); });
    list_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { Edit(false); });
    remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const auto selection = list_->GetSelection(); if (selection == wxNOT_FOUND) return;
        try {
            const auto& credential = vault_.credentials.at(selection);
            auto updated = vault_; RemoveCredential(updated, credential.id);
            if (wxMessageBox(wxS("删除凭据“") + wxString::FromUTF8(credential.name) + wxS("”？"), wxS("删除凭据"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES) return;
            vault_ = std::move(updated); RefreshList();
        } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("无法删除"), wxOK | wxICON_WARNING, this); }
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        try { ValidateCatalog(vault_); EndModal(wxID_OK); }
        catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("凭据无效"), wxOK | wxICON_WARNING, this); }
    }, wxID_OK);
}
void CredentialManagerDialog::RefreshList(const std::string& selected) {
    list_->Clear();
    for (const auto& credential : vault_.credentials) {
        const auto count = std::count_if(vault_.sessions.begin(), vault_.sessions.end(), [&](const auto& session) { return session.credential_id == credential.id; });
        list_->Append(wxString::FromUTF8(credential.name + " · " + credential.username) + wxString::Format(wxS("  （%zu 台主机）"), static_cast<std::size_t>(count)));
        if (credential.id == selected) list_->SetSelection(static_cast<int>(list_->GetCount()) - 1);
    }
    if (list_->GetSelection() == wxNOT_FOUND && list_->GetCount()) list_->SetSelection(0);
}
void CredentialManagerDialog::Edit(bool create) {
    const auto selection = list_->GetSelection(); if (!create && selection == wxNOT_FOUND) return;
    CredentialDialog dialog(this, create ? Credential{} : vault_.credentials.at(selection)); if (dialog.ShowModal() != wxID_OK) return;
    const auto credential = dialog.Result();
    if (create) vault_.credentials.push_back(credential); else vault_.credentials.at(selection) = credential;
    RefreshList(credential.id);
}
SessionDialog::SessionDialog(wxWindow* parent, Session initial, Vault vault)
    : wxDialog(parent, wxID_ANY, initial.id.empty() ? wxS("新增 SSH 会话") : wxS("编辑 SSH 会话"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), result_(std::move(initial)), vault_(std::move(vault)) {
    auto* layout = new wxBoxSizer(wxVERTICAL); auto* grid = new wxFlexGridSizer(2, 8, 12); grid->AddGrowableCol(1);
    name_ = Field(this, grid, wxS("会话名称"), result_.name);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("分组")), 0, wxALIGN_CENTER_VERTICAL);
    wxArrayString groups;
    for (const auto& session : vault_.sessions) {
        const auto group = wxString::FromUTF8(session.group);
        if (!group.empty() && groups.Index(group) == wxNOT_FOUND) groups.Add(group);
    }
    groups.Sort();
    group_ = new wxComboBox(this, wxID_ANY, wxString::FromUTF8(result_.group), wxDefaultPosition, wxDefaultSize, groups, wxCB_DROPDOWN);
    group_->SetHint(wxS("选择已有分组或输入新名称，留空为未分组"));
    grid->Add(group_, 1, wxEXPAND);
    host_ = Field(this, grid, wxS("IP / 域名"), result_.host);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("端口")), 0, wxALIGN_CENTER_VERTICAL);
    port_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, result_.port); grid->Add(port_, 1, wxEXPAND);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("登录凭据")), 0, wxALIGN_CENTER_VERTICAL);
    auto* credentials = new wxBoxSizer(wxHORIZONTAL); credential_ = new wxChoice(this, wxID_ANY);
    credentials->Add(credential_, 1, wxEXPAND | wxRIGHT, 8); auto* manage = new wxButton(this, wxID_ANY, wxS("管理凭据…")); credentials->Add(manage); grid->Add(credentials, 1, wxEXPAND);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("连接超时（秒）")), 0, wxALIGN_CENTER_VERTICAL);
    timeout_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 300, result_.timeout_seconds); grid->Add(timeout_, 1, wxEXPAND);
    layout->Add(grid, 1, wxEXPAND | wxALL, 16);
    layout->Add(new wxStaticText(this, wxID_ANY, wxS("选择已有凭据即可复用用户名、密码或私钥，无需为每台主机重复填写。")), 0, wxLEFT | wxRIGHT | wxBOTTOM, 16);
    layout->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);
    SetSizerAndFit(layout); SetMinSize(wxSize(680, 330)); RefreshCredentials(result_.credential_id);
    manage->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const auto selected = credential_->GetSelection();
        const auto previous = selected == wxNOT_FOUND ? std::string{} : vault_.credentials.at(selected).id;
        CredentialManagerDialog dialog(this, vault_); if (dialog.ShowModal() != wxID_OK) return;
        vault_ = dialog.Result(); RefreshCredentials(previous);
    });
    Bind(wxEVT_BUTTON, &SessionDialog::Accept, this, wxID_OK);
}
void SessionDialog::RefreshCredentials(const std::string& selected) {
    credential_->Clear();
    for (const auto& credential : vault_.credentials) {
        credential_->Append(wxString::FromUTF8(credential.name + " · " + credential.username) + (credential.auth == AuthMethod::Password ? wxS("（密码）") : wxS("（私钥）")));
        if (credential.id == selected) credential_->SetSelection(static_cast<int>(credential_->GetCount()) - 1);
    }
    if (credential_->GetSelection() == wxNOT_FOUND && credential_->GetCount()) credential_->SetSelection(0);
}
void SessionDialog::Accept(wxCommandEvent&) {
    try {
        auto updated = result_; if (updated.id.empty()) updated.id = RandomId();
        updated.name = Text(name_); updated.group = group_->GetValue().ToStdString(wxConvUTF8); updated.host = Text(host_);
        updated.port = static_cast<std::uint16_t>(port_->GetValue()); updated.timeout_seconds = timeout_->GetValue();
        const auto selected = credential_->GetSelection(); if (selected == wxNOT_FOUND) throw std::runtime_error("请先在“管理凭据”中新增凭据，再选择登录凭据");
        updated.credential_id = vault_.credentials.at(selected).id; ValidateSessions({updated}); FindCredential(vault_, updated.credential_id);
        result_ = std::move(updated); EndModal(wxID_OK);
    } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("会话参数无效"), wxOK | wxICON_WARNING, this); }
}
WebDavDialog::WebDavDialog(wxWindow* parent, WebDavSettings initial)
    : wxDialog(parent, wxID_ANY, wxS("WebDAV 手动备份设置"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), result_(std::move(initial)), test_timer_(this) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(this, wxID_ANY, wxS("填写 MapleStore 的 HTTPS WebDAV 地址，例如 https://nas.example/dav/。\n这些连接设置仅保存在本机，不包含在备份中。")), 0, wxALL, 16);
    auto* grid = new wxFlexGridSizer(2, 10, 12); grid->AddGrowableCol(1);
    url_ = Field(this, grid, wxS("WebDAV 地址"), result_.url);
    username_ = Field(this, grid, wxS("账号"), result_.username);
    password_ = Field(this, grid, wxS("密码"), result_.password, wxTE_PASSWORD);
    directory_ = Field(this, grid, wxS("备份子目录"), result_.directory);
    layout->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 16);
    auto* testing = new wxBoxSizer(wxHORIZONTAL);
    test_ = new wxButton(this, wxID_ANY, wxS("测试登录"));
    testing->Add(test_, 0, wxRIGHT, 12);
    test_status_ = new wxStaticText(this, wxID_ANY, wxS("验证登录和 WebDAV 目录访问，不会上传文件。"), wxDefaultPosition, wxSize(450, 56));
    test_status_->Wrap(450);
    testing->Add(test_status_, 1, wxALIGN_CENTER_VERTICAL);
    layout->Add(testing, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 16);
    layout->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 16);
    SetSizerAndFit(layout); SetMinSize(wxSize(650, -1));
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        try {
            if (test_result_.valid()) return;
            result_ = ReadSettings(); EndModal(wxID_OK);
        } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("设置无效"), wxOK | wxICON_WARNING, this); }
    }, wxID_OK);
    test_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { TestLogin(); });
    for (auto* field : {url_, username_, password_, directory_}) {
        field->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
            if (!test_result_.valid()) { test_status_->SetLabel(wxS("设置已修改，请点击“测试登录”重新验证。")); test_status_->Wrap(450); Layout(); }
            event.Skip();
        });
    }
    Bind(wxEVT_TIMER, &WebDavDialog::PollTest, this, test_timer_.GetId());
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CancelTest(); }, wxID_CANCEL);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if (test_result_.valid() && event.CanVeto()) { CancelTest(); event.Veto(); }
        else event.Skip();
    });
}
WebDavDialog::~WebDavDialog() {
    test_timer_.Stop();
    if (test_client_) test_client_->Cancel();
    if (test_result_.valid()) test_result_.wait();
}
WebDavSettings WebDavDialog::ReadSettings() const {
    auto settings = WebDavSettings{Text(url_), Text(username_), Text(password_), Text(directory_)};
    ParseEndpoint(settings);
    if (settings.username.empty() || settings.password.empty()) throw std::runtime_error("请输入 WebDAV 账号和密码");
    return settings;
}
void WebDavDialog::TestLogin() {
    if (test_result_.valid()) return;
    try {
        auto client = std::make_shared<WebDavClient>(ReadSettings());
        test_result_ = std::async(std::launch::async, [client] {
            try { client->TestLogin(); return std::string{}; }
            catch (const std::exception& error) { return std::string(error.what()); }
        });
        test_client_ = std::move(client);
        closing_ = false;
        for (auto* field : {url_, username_, password_, directory_}) field->Disable();
        FindWindow(wxID_OK)->Disable(); test_->Disable(); test_->SetLabel(wxS("正在测试…"));
        test_status_->SetLabel(wxS("正在验证账号及 WebDAV 目录访问…"));
        test_timer_.Start(80);
    } catch (const std::exception& error) {
        test_status_->SetLabel(wxString::FromUTF8(error.what())); test_status_->Wrap(450); Layout();
    }
}
void WebDavDialog::PollTest(wxTimerEvent&) {
    if (!test_result_.valid() || test_result_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
    const auto error = test_result_.get();
    test_timer_.Stop(); test_client_.reset();
    if (closing_) { EndModal(wxID_CANCEL); return; }
    for (auto* field : {url_, username_, password_, directory_}) field->Enable();
    FindWindow(wxID_OK)->Enable(); test_->Enable(); test_->SetLabel(wxS("测试登录"));
    test_status_->SetLabel(error.empty() ? wxString(wxS("登录成功，WebDAV 目录可访问。\n未上传文件；备份写入权限将在实际备份时验证。")) : wxS("测试失败：") + wxString::FromUTF8(error));
    test_status_->Wrap(450); Layout();
}
void WebDavDialog::CancelTest() {
    if (!test_result_.valid()) { EndModal(wxID_CANCEL); return; }
    closing_ = true; test_client_->Cancel();
    test_status_->SetLabel(wxS("正在取消测试，请稍候…"));
}
}
