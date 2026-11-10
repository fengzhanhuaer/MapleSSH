#include "sftp_client.hpp"
#include <iostream>
#include <stdexcept>
#include <nlohmann/json.hpp>

using namespace maple;
template<class Action> void Rejects(Action action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid file operation accepted");
}
int main() {
    try {
        Session session; Credential credential;
        Rejects([&] { SftpClient client(session, credential, ""); });
        SftpClient client(session, credential, "pinned-host-key");
        Rejects([&] { client.Submit(FileOperation::List, "bad\npath"); });
        Rejects([&] { client.Submit(FileOperation::Upload, "/target"); });
        Rejects([&] { client.Submit(FileOperation::Download, "/target", std::filesystem::path("relative")); });
        Rejects([&] { client.Snapshot("unknown"); });
        FileSnapshot sample; sample.id = "test-job"; sample.remote_path = "/目录"; sample.entries.push_back({"文件", false, true, false, 123, 456});
        const auto serialized = nlohmann::json::parse(SerializeFileSnapshot(sample));
        if (serialized.at("entries").at(0).at("size") != 123 || serialized.contains("local_path") || serialized.contains("password")) throw std::runtime_error("file task serialization invalid");
        client.Stop();
        Rejects([&] { client.Submit(FileOperation::List, "/"); });
        std::cout << "SFTP task guards PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what(); return 1; }
}
