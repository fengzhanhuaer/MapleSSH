#pragma once
#include "model.hpp"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

namespace maple {
enum class FileOperation { List, Upload, Download };
struct RemoteFile {
    std::string name;
    bool directory = false;
    bool regular = false;
    bool symlink = false;
    std::uint64_t size = 0;
    std::uint64_t modified = 0;
};
struct FileSnapshot {
    std::string id;
    FileOperation operation = FileOperation::List;
    std::string state = "running";
    std::string remote_path;
    std::string error;
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    bool truncated = false;
    std::vector<RemoteFile> entries;
};
std::string SerializeFileSnapshot(const FileSnapshot& snapshot);
class SftpClient final {
public:
    SftpClient(Session session, Credential credential, std::string pinned_key);
    ~SftpClient();
    std::string Submit(FileOperation operation, std::string remote_path, std::filesystem::path local_path = {}, bool overwrite = false);
    FileSnapshot Snapshot(std::string_view id) const;
    void Cancel(std::string_view id);
    void Stop();
private:
    struct Job {
        FileSnapshot snapshot;
        std::filesystem::path local_path;
        bool overwrite = false;
        std::stop_source cancellation;
    };
    void Execute(const std::shared_ptr<Job>& job);
    Session session_;
    Credential credential_;
    std::string pinned_key_;
    mutable std::mutex mutex_;
    std::mutex lifecycle_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::jthread worker_;
    bool stopped_ = false;
};
}
