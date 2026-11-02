#pragma once
#include "model.hpp"
#include <wx/wx.h>
#include <wx/spinctrl.h>
#include <optional>
namespace maple {
std::optional<std::string> AskMasterPassword(wxWindow* parent, bool create);
class SessionDialog : public wxDialog {
public:
    SessionDialog(wxWindow* parent, Session initial);
    Session Result() const { return result_; }
private:
    Session result_;
    wxTextCtrl* name_;
    wxTextCtrl* group_;
    wxTextCtrl* host_;
    wxSpinCtrl* port_;
    wxTextCtrl* username_;
    wxChoice* auth_;
    wxTextCtrl* password_;
    wxTextCtrl* private_key_;
    wxTextCtrl* passphrase_;
    wxSpinCtrl* timeout_;
    void Accept(wxCommandEvent& event);
};
class WebDavDialog : public wxDialog {
public:
    WebDavDialog(wxWindow* parent, WebDavSettings initial);
    WebDavSettings Result() const { return result_; }
private:
    WebDavSettings result_;
    wxTextCtrl* url_;
    wxTextCtrl* username_;
    wxTextCtrl* password_;
    wxTextCtrl* directory_;
};
}
