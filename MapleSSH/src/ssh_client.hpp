#pragma once
#include "model.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
namespace maple {
enum class SshEventKind { Output, State, HostKey };
struct SshEvent {
    SshEventKind kind;
    std::string text;
    std::string algorithm;
    std::string key;
};
class SshClient {
public:
    SshClient(Session session, Credential credential);
    ~SshClient();
    void Start(int rows = 24, int columns = 80);
    void Stop();
    bool Send(std::string text);
    void Resize(int rows, int columns);
    std::vector<SshEvent> Drain();
    void ConfirmHost(bool accepted);
    bool Connected() const { return connected_; }
private:
    void Run(std::stop_token stop);
    void Post(SshEvent event);
    Session session_;
    Credential credential_;
    std::jthread worker_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<SshEvent> events_;
    std::string input_;
    std::size_t queued_bytes_ = 0;
    std::optional<bool> host_decision_;
    int rows_ = 24;
    int columns_ = 80;
    bool resized_ = true;
    std::atomic<bool> connected_{false};
};
}
