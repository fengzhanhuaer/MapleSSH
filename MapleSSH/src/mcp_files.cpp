#include "main_frame.hpp"
#include "file_paths.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace maple {
void MainFrame::CancelMcpApprovals() {
    for (auto& [id, job] : mcp_file_jobs_) {
        if (job.approval) { job.approval->Destroy(); job.approval = nullptr; job.state = "cancelled"; }
        if (job.client && !job.backend_id.empty()) {
            try { if (job.client->Snapshot(job.backend_id).state == "running") job.client->Cancel(job.backend_id); } catch (...) { }
        }
    }
}
std::string MainFrame::McpFiles(std::string_view tool, std::string_view arguments) {
    return InvokeMcpUi([this, name = std::string(tool), input = std::string(arguments)] {
        using Json = nlohmann::json;
        const auto args = Json::parse(input);
        if (name == "get_file_job" || name == "cancel_file_job") {
            const auto id = args.at("job_id").get<std::string>();
            const auto found = mcp_file_jobs_.find(id);
            if (found == mcp_file_jobs_.end()) throw std::runtime_error("文件任务不存在或已过期");
            auto& job = found->second;
            if (name == "cancel_file_job") {
                if (job.approval) { job.approval->Destroy(); job.approval = nullptr; job.state = "cancelled"; }
                if (!job.backend_id.empty()) job.client->Cancel(job.backend_id);
            }
            if (!job.backend_id.empty()) {
                auto result = Json::parse(SerializeFileSnapshot(job.client->Snapshot(job.backend_id)));
                result["job_id"] = id; result["session_id"] = job.session_id; return result.dump();
            }
            return Json{{"job_id", id}, {"session_id", job.session_id}, {"state", job.state}, {"error", job.error}}.dump();
        }
        const auto session_id = args.at("session_id").get<std::string>();
        const auto found = std::find_if(tabs_.begin(), tabs_.end(), [&](const auto& tab) { return tab->session.id == session_id; });
        if (found == tabs_.end() || !(*found)->client->Connected()) throw std::runtime_error("请先打开并连接指定 SSH 会话");
        auto client = (*found)->client->Files();
        const auto remote = name == "list_directory" ? args.value("path", std::string(".")) : args.at("remote_path").get<std::string>();
        ValidateRemotePath(remote);
        const auto operation = name == "list_directory" ? FileOperation::List : name == "upload_file" ? FileOperation::Upload : FileOperation::Download;
        std::filesystem::path local;
        const bool overwrite = args.value("overwrite", false);
        if (operation != FileOperation::List) {
            const auto text = args.at("local_path").get<std::string>();
            local = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
            ValidateLocalFilePath(local);
            if (!remote.starts_with('/')) throw std::runtime_error("MCP 上传下载必须指定远程绝对路径");
        }
        if (mcp_file_jobs_.size() >= 64) {
            auto expired = std::find_if(mcp_file_jobs_.begin(), mcp_file_jobs_.end(), [](const auto& entry) {
                const auto& job = entry.second;
                if (job.approval) return false;
                if (job.backend_id.empty()) return true;
                try { return job.client->Snapshot(job.backend_id).state != "running"; } catch (...) { return true; }
            });
            if (expired == mcp_file_jobs_.end()) throw std::runtime_error("待处理文件任务过多，请完成或取消后再试");
            mcp_file_jobs_.erase(expired);
        }
        const auto id = RandomId();
        McpFileJob job; job.session_id = session_id; job.client = client;
        if (operation == FileOperation::List) {
            job.backend_id = client->Submit(operation, remote); job.state = "running";
            mcp_file_jobs_.emplace(id, std::move(job));
            return Json{{"job_id", id}, {"session_id", session_id}, {"state", "running"}}.dump();
        }
        auto* approval = new wxDialog(this, wxID_ANY, wxS("MCP 文件访问批准"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        job.approval = approval; mcp_file_jobs_.emplace(id, std::move(job));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        const auto message = wxString(operation == FileOperation::Upload ? wxS("AI 请求上传本机文件") : wxS("AI 请求下载远程文件")) + wxS("\n\n主机：") +
            wxString::FromUTF8((*found)->session.name + " (" + (*found)->session.host + ")") + wxS("\n远程：") + wxString::FromUTF8(remote) +
            wxS("\n本机：") + wxString(local.wstring()) + (overwrite ? wxS("\n\n本次允许覆盖同名普通文件。") : wxS("\n\n本次不允许覆盖已有文件。")) + wxS("\n只批准这一次操作；拒绝不会传输文件。");
        auto* details = new wxTextCtrl(approval, wxID_ANY, message, wxDefaultPosition, FromDIP(wxSize(500, 240)), wxTE_MULTILINE | wxTE_READONLY);
        layout->Add(details, 1, wxEXPAND | wxALL, FromDIP(10));
        auto* buttons = new wxBoxSizer(wxHORIZONTAL); buttons->AddStretchSpacer();
        auto* allow = new wxButton(approval, wxID_YES, wxS("允许本次"));
        auto* deny = new wxButton(approval, wxID_NO, wxS("拒绝")); deny->SetDefault();
        buttons->Add(allow, 0, wxRIGHT, FromDIP(8)); buttons->Add(deny, 0);
        layout->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10)); approval->SetSizerAndFit(layout);
        auto finish = [this, id, operation, remote, local, overwrite](bool allowed) {
            const auto pending = mcp_file_jobs_.find(id);
            if (pending == mcp_file_jobs_.end() || !pending->second.approval) return;
            auto& job = pending->second; auto* dialog = job.approval; job.approval = nullptr;
            try {
                if (!allowed || !mcp_accepting_) job.state = "cancelled";
                else {
                    const auto current = std::find_if(tabs_.begin(), tabs_.end(), [&](const auto& tab) { return tab->session.id == job.session_id; });
                    if (current == tabs_.end() || !(*current)->client->Connected() || (*current)->client->Files() != job.client) throw std::runtime_error("会话已断开或重连，请重新申请本次传输");
                    job.backend_id = job.client->Submit(operation, remote, local, overwrite); job.state = "running";
                }
            } catch (const std::exception& error) { job.state = "failed"; job.error = error.what(); }
            dialog->Destroy();
        };
        allow->Bind(wxEVT_BUTTON, [finish](wxCommandEvent&) { finish(true); });
        deny->Bind(wxEVT_BUTTON, [finish](wxCommandEvent&) { finish(false); });
        approval->Bind(wxEVT_CLOSE_WINDOW, [finish](wxCloseEvent&) { finish(false); });
        approval->SetEscapeId(wxID_NO); approval->CentreOnParent(); approval->Show();
        return Json{{"job_id", id}, {"session_id", session_id}, {"state", "awaiting_approval"}, {"message", "请用户在 MapleSSH 界面核对本机/远程路径并批准；用 get_file_job 查询结果。"}}.dump();
    });
}
}
