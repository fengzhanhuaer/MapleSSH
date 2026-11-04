#pragma once
#include "model.hpp"
#include "storage.hpp"
#include "host_keys.hpp"
#include "ssh_client.hpp"
#include "terminal.hpp"
#include "webdav.hpp"
#include <wx/wx.h>
#include <wx/aui/auibook.h>
#include <wx/treectrl.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <atomic>
#include <memory>
#include <thread>
namespace maple {
class MainFrame : public wxFrame {
public:
    MainFrame(Storage storage, Vault vault);
    ~MainFrame() override;
private:
    struct Tab {
        Session session;
        wxPanel* page = nullptr;
        TerminalPanel* terminal = nullptr;
        wxTextCtrl* command = nullptr;
        wxScrolledWindow* shortcuts = nullptr;
        std::unique_ptr<SshClient> client;
    };
    enum class JobKind { Save, Backup, List, Download };
    struct Job {
        JobKind kind;
        std::atomic<bool> done{false};
        std::string error;
        std::string name;
        Vault vault;
        std::vector<std::string> names;
        Vault restored;
        std::shared_ptr<WebDavClient> client;
    };
    void BuildTree();
    void ToggleSidebar();
    std::string SelectedId() const;
    void Edit(bool create);
    void Delete();
    void GroupMenu(wxTreeEvent& event);
    void RenameSelectedGroup();
    void ManageCredentials();
    void ConnectTab(Tab& tab, const Session& session);
    void ConnectSelected();
    void BuildCommandInput(Tab& tab);
    void RefreshQuickCommands();
    void EditQuickCommand(const std::string& id, const std::string& initial = {});
    void DeleteQuickCommand(const std::string& id);
    bool ExecuteCommands(Tab& tab, std::string_view text, bool confirm_multiline);
    Tab* ActiveTab();
    void Settings();
    void Backup();
    void Restore();
    void StartDownload(const std::string& name);
    void Save(Vault updated, bool recovery_copy = false);
    void Launch(const std::shared_ptr<Job>& job, std::function<void(Job&)> work);
    void Poll(wxTimerEvent& event);
    void CompleteJob();
    void Close(wxCloseEvent& event);
    void ShowError(const std::exception& error);
    Storage storage_;
    Vault vault_;
    HostKeys keys_;
    wxSplitterWindow* sidebar_splitter_ = nullptr;
    int sidebar_width_ = 245;
    wxTreeCtrl* tree_ = nullptr;
    wxAuiNotebook* notebook_ = nullptr;
    wxTimer timer_;
    std::vector<std::unique_ptr<Tab>> tabs_;
    std::shared_ptr<Job> job_;
    std::jthread background_;
    bool polling_ = false;
};
}
