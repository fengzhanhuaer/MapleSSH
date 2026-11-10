#pragma once
#include "sftp_client.hpp"
#include <wx/wx.h>
#include <wx/listctrl.h>
#include <functional>

namespace maple {
class FileBrowserPanel final : public wxPanel {
public:
    FileBrowserPanel(wxWindow* parent, std::function<std::shared_ptr<SftpClient>()> files,
        std::function<bool(std::string)> sender, std::function<void()> hide);
    void Poll(bool connected);
private:
    void Browse(std::string path);
    void Upload();
    void Download();
    void EnterDirectory();
    void Watch(const std::string& id);
    void ShowFiles(const FileSnapshot& snapshot);
    std::shared_ptr<SftpClient> ActiveFiles() const;
    const RemoteFile* Selected() const;
    void Error(const std::exception& error);
    std::function<std::shared_ptr<SftpClient>()> files_;
    std::function<bool(std::string)> sender_;
    std::shared_ptr<SftpClient> client_;
    wxTextCtrl* path_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxStaticText* status_ = nullptr;
    wxGauge* progress_ = nullptr;
    wxButton* cancel_ = nullptr;
    std::vector<wxWindow*> actions_;
    std::vector<RemoteFile> entries_;
    std::string directory_ = ".";
    std::string watching_;
    bool connected_ = false;
};
}
