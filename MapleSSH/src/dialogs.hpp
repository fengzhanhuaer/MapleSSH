#pragma once
#include "model.hpp"
#include "webdav.hpp"
#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <wx/combobox.h>
#include <future>
#include <memory>
namespace maple {
class QuickCommandDialog : public wxDialog {
public:
    QuickCommandDialog(wxWindow* parent, QuickCommand initial);
    QuickCommand Result() const { return result_; }
private:
    QuickCommand result_;
    wxTextCtrl* name_;
    wxTextCtrl* command_;
};
class CredentialDialog : public wxDialog {
public:
    CredentialDialog(wxWindow* parent, Credential initial);
    Credential Result() const { return result_; }
private:
    Credential result_;
    wxTextCtrl* name_;
    wxTextCtrl* username_;
    wxChoice* auth_;
    wxTextCtrl* password_;
    wxTextCtrl* private_key_;
    wxTextCtrl* passphrase_;
    void Accept(wxCommandEvent& event);
};
class CredentialManagerDialog : public wxDialog {
public:
    CredentialManagerDialog(wxWindow* parent, Vault initial);
    Vault Result() const { return vault_; }
private:
    Vault vault_;
    wxListBox* list_;
    void RefreshList(const std::string& selected = {});
    void Edit(bool create);
};
class SessionDialog : public wxDialog {
public:
    SessionDialog(wxWindow* parent, Session initial, Vault vault);
    Session Result() const { return result_; }
    Vault ResultVault() const { return vault_; }
private:
    Session result_;
    Vault vault_;
    wxTextCtrl* name_;
    wxComboBox* group_;
    wxTextCtrl* host_;
    wxSpinCtrl* port_;
    wxChoice* credential_;
    wxSpinCtrl* timeout_;
    void RefreshCredentials(const std::string& selected);
    void Accept(wxCommandEvent& event);
};
class WebDavDialog : public wxDialog {
public:
    WebDavDialog(wxWindow* parent, WebDavSettings initial);
    ~WebDavDialog() override;
    WebDavSettings Result() const { return result_; }
private:
    WebDavSettings result_;
    wxTextCtrl* url_;
    wxTextCtrl* username_;
    wxTextCtrl* password_;
    wxTextCtrl* directory_;
    wxButton* test_;
    wxStaticText* test_status_;
    wxTimer test_timer_;
    std::shared_ptr<WebDavClient> test_client_;
    std::future<std::string> test_result_;
    bool closing_ = false;
    WebDavSettings ReadSettings() const;
    void TestLogin();
    void PollTest(wxTimerEvent& event);
    void CancelTest();
};
}
