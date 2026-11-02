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
std::optional<std::string> AskMasterPassword(wxWindow* parent, bool create) {
    wxDialog dialog(parent, wxID_ANY, create ? wxS("创建 MapleSSH 主密码") : wxS("解锁 MapleSSH"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* explanation = new wxStaticText(&dialog, wxID_ANY, create ?
        wxS("主密码用于保护本机凭据和云端备份。\n请妥善保存，遗失后无法恢复。至少 12 个字符。") : wxS("输入主密码，解锁本机 SSH 会话资料。"));
    layout->Add(explanation, 0, wxALL, 16);
    auto* grid = new wxFlexGridSizer(2, 10, 12);
    grid->AddGrowableCol(1);
    auto* password = Field(&dialog, grid, wxS("主密码"), "", wxTE_PASSWORD);
    wxTextCtrl* confirmation = nullptr;
    if (create) confirmation = Field(&dialog, grid, wxS("再次输入"), "", wxTE_PASSWORD);
    layout->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 16);
    layout->Add(dialog.CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 16);
    dialog.SetSizerAndFit(layout);
    dialog.SetMinSize(wxSize(480, -1));
    dialog.Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
        if (password->GetValue().empty() || (create && password->GetValue().length() < 12)) {
            wxMessageBox(wxS("请输入有效主密码；新主密码至少 12 个字符。"), "MapleSSH", wxOK | wxICON_WARNING, &dialog); return;
        }
        if (confirmation && confirmation->GetValue() != password->GetValue()) {
            wxMessageBox(wxS("两次输入的密码不一致"), "MapleSSH", wxOK | wxICON_WARNING, &dialog); return;
        }
        dialog.EndModal(wxID_OK);
    }, wxID_OK);
    password->SetFocus();
    if (dialog.ShowModal() != wxID_OK) return std::nullopt;
    return Text(password);
}
SessionDialog::SessionDialog(wxWindow* parent, Session initial)
    : wxDialog(parent, wxID_ANY, initial.id.empty() ? wxS("新增 SSH 会话") : wxS("编辑 SSH 会话"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), result_(std::move(initial)) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    auto* grid = new wxFlexGridSizer(2, 8, 12);
    grid->AddGrowableCol(1);
    name_ = Field(this, grid, wxS("会话名称"), result_.name);
    group_ = Field(this, grid, wxS("分组"), result_.group);
    host_ = Field(this, grid, wxS("IP / 域名"), result_.host);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("端口")), 0, wxALIGN_CENTER_VERTICAL);
    port_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 65535, result_.port);
    grid->Add(port_, 1, wxEXPAND);
    username_ = Field(this, grid, wxS("用户名"), result_.username);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("认证方式")), 0, wxALIGN_CENTER_VERTICAL);
    auth_ = new wxChoice(this, wxID_ANY);
    auth_->Append(wxS("密码")); auth_->Append(wxS("私钥"));
    auth_->SetSelection(result_.auth == AuthMethod::Password ? 0 : 1);
    grid->Add(auth_, 1, wxEXPAND);
    password_ = Field(this, grid, wxS("SSH 密码"), result_.password, wxTE_PASSWORD);
    passphrase_ = Field(this, grid, wxS("私钥口令"), result_.passphrase, wxTE_PASSWORD);
    grid->Add(new wxStaticText(this, wxID_ANY, wxS("连接超时（秒）")), 0, wxALIGN_CENTER_VERTICAL);
    timeout_ = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 300, result_.timeout_seconds);
    grid->Add(timeout_, 1, wxEXPAND);
    layout->Add(grid, 0, wxEXPAND | wxALL, 16);
    auto* key_header = new wxBoxSizer(wxHORIZONTAL);
    key_header->Add(new wxStaticText(this, wxID_ANY, wxS("私钥内容（加密存入配置，不保留外部路径）")), 1, wxALIGN_CENTER_VERTICAL);
    auto* import = new wxButton(this, wxID_ANY, wxS("导入私钥…"));
    key_header->Add(import);
    layout->Add(key_header, 0, wxEXPAND | wxLEFT | wxRIGHT, 16);
    private_key_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(result_.private_key), wxDefaultPosition, wxSize(560, 130), wxTE_MULTILINE | wxTE_DONTWRAP);
    private_key_->SetFont(wxFont(wxFontInfo(9).Family(wxFONTFAMILY_TELETYPE)));
    layout->Add(private_key_, 1, wxEXPAND | wxALL, 16);
    layout->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 16);
    SetSizerAndFit(layout);
    SetMinSize(wxSize(640, 620));
    const auto toggle = [this] { password_->Enable(auth_->GetSelection() == 0); passphrase_->Enable(auth_->GetSelection() == 1); private_key_->Enable(auth_->GetSelection() == 1); };
    toggle();
    auth_->Bind(wxEVT_CHOICE, [toggle](wxCommandEvent&) { toggle(); });
    import->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxFileDialog file(this, wxS("导入 SSH 私钥"), wxEmptyString, wxEmptyString, wxS("所有文件 (*.*)|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (file.ShowModal() != wxID_OK) return;
        try {
            const auto bytes = ReadBytes(std::filesystem::path(file.GetPath().ToStdWstring()));
            if (bytes.size() > 1024 * 1024) throw std::runtime_error("私钥超过 1 MiB");
            const auto text = wxString::FromUTF8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (text.empty()) throw std::runtime_error("仅支持文本格式的 SSH 私钥");
            private_key_->SetValue(text);
            auth_->SetSelection(1); password_->Disable(); passphrase_->Enable(); private_key_->Enable();
        } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("导入失败"), wxOK | wxICON_ERROR, this); }
    });
    Bind(wxEVT_BUTTON, &SessionDialog::Accept, this, wxID_OK);
}
void SessionDialog::Accept(wxCommandEvent&) {
    try {
        auto updated = result_;
        if (updated.id.empty()) updated.id = RandomId();
        updated.name = Text(name_); updated.group = Text(group_); updated.host = Text(host_);
        updated.port = static_cast<std::uint16_t>(port_->GetValue()); updated.username = Text(username_);
        updated.auth = auth_->GetSelection() == 0 ? AuthMethod::Password : AuthMethod::PrivateKey;
        updated.password = updated.auth == AuthMethod::Password ? Text(password_) : "";
        updated.private_key = updated.auth == AuthMethod::PrivateKey ? Text(private_key_) : "";
        updated.passphrase = updated.auth == AuthMethod::PrivateKey ? Text(passphrase_) : "";
        updated.timeout_seconds = timeout_->GetValue();
        ValidateSessions({updated});
        result_ = std::move(updated);
        EndModal(wxID_OK);
    } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("会话参数无效"), wxOK | wxICON_WARNING, this); }
}
WebDavDialog::WebDavDialog(wxWindow* parent, WebDavSettings initial)
    : wxDialog(parent, wxID_ANY, wxS("WebDAV 手动备份设置"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), result_(std::move(initial)) {
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(new wxStaticText(this, wxID_ANY, wxS("填写 MapleStore 的 HTTPS WebDAV 地址，例如 https://nas.example/dav/。\n这些连接设置仅保存在本机，不包含在备份中。")), 0, wxALL, 16);
    auto* grid = new wxFlexGridSizer(2, 10, 12); grid->AddGrowableCol(1);
    url_ = Field(this, grid, wxS("WebDAV 地址"), result_.url);
    username_ = Field(this, grid, wxS("账号"), result_.username);
    password_ = Field(this, grid, wxS("密码"), result_.password, wxTE_PASSWORD);
    directory_ = Field(this, grid, wxS("备份子目录"), result_.directory);
    layout->Add(grid, 1, wxEXPAND | wxLEFT | wxRIGHT, 16);
    layout->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, 16);
    SetSizerAndFit(layout); SetMinSize(wxSize(650, -1));
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        try {
            auto settings = WebDavSettings{Text(url_), Text(username_), Text(password_), Text(directory_)};
            ParseEndpoint(settings);
            if (settings.username.empty() || settings.password.empty()) throw std::runtime_error("请输入 WebDAV 账号和密码");
            result_ = std::move(settings); EndModal(wxID_OK);
        } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("设置无效"), wxOK | wxICON_WARNING, this); }
    }, wxID_OK);
}
}
