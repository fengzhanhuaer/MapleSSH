#include "sftp_client.hpp"
#include "ssh_transport.hpp"
#include "crypto.hpp"
#include "file_paths.hpp"
#include <libssh2_sftp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace maple {
std::string SerializeFileSnapshot(const FileSnapshot& snapshot) {
    auto entries = nlohmann::json::array();
    for (const auto& entry : snapshot.entries) entries.push_back({{"name", entry.name}, {"directory", entry.directory}, {"regular", entry.regular},
        {"symlink", entry.symlink}, {"size", entry.size}, {"modified", entry.modified}});
    return nlohmann::json{{"job_id", snapshot.id}, {"state", snapshot.state}, {"operation", snapshot.operation == FileOperation::List ? "list" : snapshot.operation == FileOperation::Upload ? "upload" : "download"},
        {"remote_path", snapshot.remote_path}, {"error", snapshot.error}, {"bytes", snapshot.bytes}, {"total", snapshot.total}, {"truncated", snapshot.truncated}, {"entries", std::move(entries)}}.dump();
}
namespace {
struct RemoteConnection {
    ssh::Connection transport;
    LIBSSH2_SFTP* sftp = nullptr;
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    ~RemoteConnection() {
        if (transport.socket != INVALID_SOCKET) { shutdown(transport.socket, SD_BOTH); closesocket(transport.socket); transport.socket = INVALID_SOCKET; }
        if (handle) libssh2_sftp_close_handle(handle);
        if (sftp) libssh2_sftp_shutdown(sftp);
    }
};
struct LocalFile {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path temporary;
    ~LocalFile() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
    }
    void Close() {
        if (handle != INVALID_HANDLE_VALUE) { const auto closed = CloseHandle(handle); handle = INVALID_HANDLE_VALUE; if (!closed) throw std::runtime_error("关闭本机文件失败"); }
    }
};
struct RemoteTemporary {
    LIBSSH2_SFTP* sftp;
    std::string path;
    bool published = false;
    ~RemoteTemporary() { if (!published) libssh2_sftp_unlink_ex(sftp, path.data(), static_cast<unsigned int>(path.size())); }
};
void Require(int result, const char* message) { if (result < 0) throw std::runtime_error(message); }
}
SftpClient::SftpClient(Session session, Credential credential, std::string pinned_key)
    : session_(std::move(session)), credential_(std::move(credential)), pinned_key_(std::move(pinned_key)) {
    if (pinned_key_.empty()) throw std::runtime_error("SFTP 需要已认证终端的主机密钥");
    ssh::Initialize();
}
SftpClient::~SftpClient() { Stop(); }
std::string SftpClient::Submit(FileOperation operation, std::string remote_path, std::filesystem::path local_path, bool overwrite) {
    const std::lock_guard lifecycle(lifecycle_);
    ValidateRemotePath(remote_path);
    if (operation != FileOperation::List) ValidateLocalFilePath(local_path);
    auto job = std::make_shared<Job>();
    job->snapshot.id = RandomId(); job->snapshot.operation = operation; job->snapshot.remote_path = std::move(remote_path);
    job->local_path = std::move(local_path); job->overwrite = overwrite;
    {
        const std::lock_guard lock(mutex_);
        if (stopped_) throw std::runtime_error("文件连接已关闭");
        if (!jobs_.empty() && jobs_.back()->snapshot.state == "running") throw std::runtime_error("本会话已有文件任务，请等待完成或取消");
        if (jobs_.size() >= 32) jobs_.erase(jobs_.begin());
        jobs_.push_back(job);
    }
    try { worker_ = std::jthread([this, job] { Execute(job); }); }
    catch (...) { const std::lock_guard lock(mutex_); jobs_.pop_back(); throw; }
    return job->snapshot.id;
}
FileSnapshot SftpClient::Snapshot(std::string_view id) const {
    const std::lock_guard lock(mutex_);
    for (const auto& job : jobs_) if (job->snapshot.id == id) return job->snapshot;
    throw std::runtime_error("文件任务不存在或已过期");
}
void SftpClient::Cancel(std::string_view id) {
    const std::lock_guard lock(mutex_);
    for (const auto& job : jobs_) if (job->snapshot.id == id) { job->cancellation.request_stop(); return; }
    throw std::runtime_error("文件任务不存在或已过期");
}
void SftpClient::Stop() {
    const std::lock_guard lifecycle(lifecycle_);
    { const std::lock_guard lock(mutex_); stopped_ = true; for (const auto& job : jobs_) job->cancellation.request_stop(); }
    if (worker_.joinable()) worker_.join();
}
void SftpClient::Execute(const std::shared_ptr<Job>& job) {
    const auto stop = job->cancellation.get_token();
    auto update = [this, &job](auto change) { const std::lock_guard lock(mutex_); change(job->snapshot); };
    bool committing = false;
    try {
        RemoteConnection remote;
        auto& connection = remote.transport;
        ssh::Connect(connection, session_, stop);
        connection.session = libssh2_session_init();
        if (!connection.session) throw std::runtime_error("无法创建 SFTP SSH 会话");
        libssh2_session_set_blocking(connection.session, 0);
        auto retry = [&](const std::function<int()>& operation) { return ssh::Retry(connection, stop, session_.timeout_seconds, operation); };
        Require(retry([&] { return libssh2_session_handshake(connection.session, connection.socket); }), "SFTP SSH 握手失败");
        std::size_t key_size = 0; int key_type = 0;
        const auto* key = libssh2_session_hostkey(connection.session, &key_size, &key_type);
        if (!key || key_size > 65536 || std::string_view(key, key_size) != pinned_key_) throw std::runtime_error("SFTP 主机密钥与当前终端不一致，已阻止认证");
        ssh::Authenticate(connection, credential_, session_.timeout_seconds, stop);
        Require(retry([&] { remote.sftp = libssh2_sftp_init(connection.session); return remote.sftp ? 0 : libssh2_session_last_errno(connection.session); }), "服务器不支持 SFTP 或无访问权限");
        auto canonical = [&](const std::string& path) {
            std::array<char, 4097> name{};
            const auto count = retry([&] { return libssh2_sftp_realpath(remote.sftp, path.c_str(), name.data(), static_cast<unsigned int>(name.size() - 1)); });
            Require(count, "无法解析远程目录，请检查路径和权限");
            if (!count || count >= static_cast<int>(name.size())) throw std::runtime_error("远程目录过长或为空");
            auto result = std::string(name.data(), count); ValidateRemotePath(result); return result;
        };
        auto close_handle = [&] {
            Require(retry([&] { return libssh2_sftp_close_handle(remote.handle); }), "关闭远程文件失败，任务未提交");
            remote.handle = nullptr;
        };
        auto open_handle = [&](const std::string& path, unsigned long flags, int type) {
            Require(retry([&] {
                remote.handle = libssh2_sftp_open_ex(remote.sftp, path.data(), static_cast<unsigned int>(path.size()), flags, 0600, type);
                return remote.handle ? 0 : libssh2_session_last_errno(connection.session);
            }), "无法打开远程文件或目录，请检查路径和权限");
        };
        const auto operation = job->snapshot.operation;
        const auto requested = job->snapshot.remote_path;
        if (operation == FileOperation::List) {
            const auto path = canonical(requested);
            open_handle(path, 0, LIBSSH2_SFTP_OPENDIR);
            std::vector<RemoteFile> entries;
            std::size_t name_bytes = 0;
            bool truncated = false;
            for (std::size_t scanned = 0;; ++scanned) {
                if (scanned >= 5000) { truncated = true; break; }
                ssh::Cancelled(stop);
                std::array<char, 4097> name{}; LIBSSH2_SFTP_ATTRIBUTES attributes{};
                const auto count = retry([&] { return libssh2_sftp_readdir_ex(remote.handle, name.data(), static_cast<unsigned int>(name.size() - 1), nullptr, 0, &attributes); });
                Require(count, "读取远程目录失败");
                if (!count) break;
                const auto filename = std::string(name.data(), count);
                if (filename == "." || filename == "..") continue;
                try { JoinRemotePath(path, filename); } catch (const std::exception&) { truncated = true; continue; }
                if (entries.size() >= 4000 || name_bytes + filename.size() > 512 * 1024) { truncated = true; break; }
                name_bytes += filename.size();
                const bool permissions = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0;
                entries.push_back({filename, permissions && LIBSSH2_SFTP_S_ISDIR(attributes.permissions), permissions && LIBSSH2_SFTP_S_ISREG(attributes.permissions),
                    permissions && LIBSSH2_SFTP_S_ISLNK(attributes.permissions), (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) ? attributes.filesize : 0,
                    (attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? attributes.mtime : 0});
            }
            close_handle();
            std::sort(entries.begin(), entries.end(), [](const auto& first, const auto& second) { return first.directory != second.directory ? first.directory : first.name < second.name; });
            update([&](auto& state) { state.remote_path = path; state.entries = std::move(entries); state.truncated = truncated; });
        } else {
            auto path = JoinRemotePath(canonical(RemoteParent(requested)), requested.substr(requested.find_last_of('/') == std::string::npos ? 0 : requested.find_last_of('/') + 1));
            update([&](auto& state) { state.remote_path = path; });
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            const auto existing = retry([&] { return libssh2_sftp_lstat(remote.sftp, path.c_str(), &attributes); });
            const bool exists = existing == 0;
            if (!exists && libssh2_sftp_last_error(remote.sftp) != LIBSSH2_FX_NO_SUCH_FILE) throw std::runtime_error("无法检查目标文件，请检查路径和权限");
            if (exists && (!(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISREG(attributes.permissions)))
                throw std::runtime_error("只支持普通文件，不上传覆盖或下载目录、符号链接及特殊文件");
            LocalFile local;
            std::array<char, 65536> buffer{};
            if (operation == FileOperation::Upload) {
                if (exists && !job->overwrite) throw std::runtime_error("远程文件已存在，请明确确认覆盖");
                local.handle = CreateFileW(job->local_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                if (local.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("无法读取本机文件");
                BY_HANDLE_FILE_INFORMATION information{};
                if (!GetFileInformationByHandle(local.handle, &information) || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) throw std::runtime_error("上传源必须是本机普通文件");
                const auto total = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
                update([&](auto& state) { state.total = total; });
                const auto temporary = JoinRemotePath(RemoteParent(path), ".maplessh-upload-" + job->snapshot.id + ".tmp");
                open_handle(temporary, LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, LIBSSH2_SFTP_OPENFILE);
                RemoteTemporary cleanup{remote.sftp, temporary};
                std::uint64_t transferred = 0;
                for (;;) {
                    ssh::Cancelled(stop);
                    DWORD count = 0;
                    if (!ReadFile(local.handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) throw std::runtime_error("读取本机文件失败");
                    if (!count) break;
                    std::size_t offset = 0;
                    while (offset < count) {
                        const auto written = retry([&] { return static_cast<int>(libssh2_sftp_write(remote.handle, buffer.data() + offset, count - offset)); });
                        if (written <= 0) throw std::runtime_error("SFTP 上传失败");
                        offset += written; transferred += written;
                        update([&](auto& state) { state.bytes = transferred; });
                    }
                }
                if (transferred != total) throw std::runtime_error("上传源文件大小在传输过程中发生变化");
                close_handle(); ssh::Cancelled(stop);
                committing = true;
                Require(retry([&] {
                    if (job->overwrite && exists) return libssh2_sftp_posix_rename_ex(remote.sftp, temporary.data(), temporary.size(), path.data(), path.size());
                    return libssh2_sftp_rename_ex(remote.sftp, temporary.data(), static_cast<unsigned int>(temporary.size()), path.data(), static_cast<unsigned int>(path.size()), 0);
                }), "上传提交失败（目标已存在、权限不足或服务器不支持原子覆盖），原文件未主动删除");
                cleanup.published = true; committing = false;
            } else {
                if (!exists) throw std::runtime_error("远程文件不存在");
                if (!job->overwrite && GetFileAttributesW(job->local_path.c_str()) != INVALID_FILE_ATTRIBUTES) throw std::runtime_error("本机文件已存在，请明确确认覆盖");
                local.temporary = job->local_path.parent_path() / (L".maplessh-download-" + std::filesystem::path(job->snapshot.id).wstring() + L".tmp");
                local.handle = CreateFileW(local.temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (local.handle == INVALID_HANDLE_VALUE) { local.temporary.clear(); throw std::runtime_error("无法创建本机临时文件，请检查下载目录"); }
                open_handle(path, LIBSSH2_FXF_READ, LIBSSH2_SFTP_OPENFILE);
                LIBSSH2_SFTP_ATTRIBUTES opened{};
                Require(retry([&] { return libssh2_sftp_fstat(remote.handle, &opened); }), "无法检查远程文件大小");
                if (!(opened.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) || !LIBSSH2_SFTP_S_ISREG(opened.permissions) || !(opened.flags & LIBSSH2_SFTP_ATTR_SIZE)) throw std::runtime_error("下载源必须是具有已知大小的普通文件");
                update([&](auto& state) { state.total = opened.filesize; });
                std::uint64_t transferred = 0;
                for (;;) {
                    ssh::Cancelled(stop);
                    const auto count = retry([&] { return static_cast<int>(libssh2_sftp_read(remote.handle, buffer.data(), buffer.size())); });
                    Require(count, "SFTP 下载读取失败"); if (!count) break;
                    DWORD written = 0;
                    if (!WriteFile(local.handle, buffer.data(), count, &written, nullptr) || written != static_cast<DWORD>(count)) throw std::runtime_error("本机文件写入失败");
                    transferred += count; update([&](auto& state) { state.bytes = transferred; });
                }
                if (transferred != opened.filesize) throw std::runtime_error("远程文件大小发生变化，下载未提交");
                close_handle();
                if (!FlushFileBuffers(local.handle)) throw std::runtime_error("无法将下载内容写入磁盘");
                local.Close(); ssh::Cancelled(stop);
                if (!MoveFileExW(local.temporary.c_str(), job->local_path.c_str(), MOVEFILE_WRITE_THROUGH | (job->overwrite ? MOVEFILE_REPLACE_EXISTING : 0))) throw std::runtime_error("下载提交失败（目标已存在或无写入权限）");
                local.temporary.clear();
            }
        }
        update([](auto& state) { state.state = "succeeded"; });
    } catch (const std::exception& error) {
        update([&](auto& state) {
            state.state = stop.stop_requested() && !committing ? "cancelled" : "failed";
            state.error = committing ? std::string("上传提交结果未确认，请检查远程目标，不要自动重试：") + error.what() : error.what();
        });
    }
}
}
