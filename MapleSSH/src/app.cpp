#include "main_frame.hpp"
#include "dialogs.hpp"
#include <wx/progdlg.h>
#include <wx/utils.h>
#include <windows.h>
#include <future>
#include <filesystem>

namespace maple {
class Application final : public wxApp {
public:
    bool OnInit() override;
    int OnExit() override;
private:
    HANDLE lock_ = INVALID_HANDLE_VALUE;
    std::unique_ptr<wxTimer> smoke_timer_;
    std::filesystem::path smoke_directory_;
};

bool Application::OnInit() {
    SetAppName("MapleSSH");
    SetVendorName("MapleSSH");
    try {
        const bool smoke = argc == 2 && wxString(argv[1]) == "--smoke-test";
        if (argc > 1 && !smoke) throw std::runtime_error("不支持的启动参数");
        const auto directory = smoke ? std::filesystem::temp_directory_path() / ("maplessh-smoke-" + RandomId()) : ExecutableDirectory() / "config";
        if (smoke) smoke_directory_ = directory;
        Storage storage(directory);
        lock_ = CreateFileW((directory / ".lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (lock_ == INVALID_HANDLE_VALUE) throw std::runtime_error("该配置已由另一 MapleSSH 实例打开，或程序目录不可写");
        Vault vault;
        std::string master;
        if (!smoke) {
            const bool create = !storage.Exists();
            for (;;) {
                const auto supplied = AskMasterPassword(nullptr, create);
                if (!supplied) return false;
                master = *supplied;
                try {
                    auto future = std::async(std::launch::async, [&] {
                        if (create) { Vault initial; storage.Save(initial, master); return initial; }
                        return storage.Load(master);
                    });
                    wxProgressDialog progress("MapleSSH", create ? wxS("正在初始化加密配置…") : wxS("正在解锁配置…"), 100, nullptr, wxPD_APP_MODAL | wxPD_SMOOTH);
                    while (future.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready) progress.Pulse();
                    vault = future.get();
                    break;
                } catch (const std::exception& error) { wxMessageBox(wxString::FromUTF8(error.what()), wxS("无法打开配置"), wxOK | wxICON_ERROR); }
            }
        }
        auto* frame = new MainFrame(std::move(storage), std::move(vault), std::move(master));
        SetTopWindow(frame); frame->Show();
        if (smoke) {
            smoke_timer_ = std::make_unique<wxTimer>(this);
            Bind(wxEVT_TIMER, [this, frame](wxTimerEvent&) { smoke_timer_->Stop(); frame->Destroy(); }, smoke_timer_->GetId());
            smoke_timer_->StartOnce(15000);
        }
        return true;
    } catch (const std::exception& error) {
        wxMessageBox(wxString::FromUTF8(error.what()), wxS("MapleSSH 启动失败"), wxOK | wxICON_ERROR);
        return false;
    }
}
int Application::OnExit() {
    if (lock_ != INVALID_HANDLE_VALUE) { CloseHandle(lock_); lock_ = INVALID_HANDLE_VALUE; }
    if (!smoke_directory_.empty()) {
        const auto parent = std::filesystem::weakly_canonical(smoke_directory_.parent_path());
        const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
        if (parent == temporary && smoke_directory_.filename().wstring().starts_with(L"maplessh-smoke-")) {
            std::error_code ignored; std::filesystem::remove_all(smoke_directory_, ignored);
        }
    }
    return wxApp::OnExit();
}
}
wxIMPLEMENT_APP(maple::Application);
