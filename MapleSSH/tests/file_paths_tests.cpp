#include "file_paths.hpp"
#include <iostream>
#include <stdexcept>

using namespace maple;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Action> void Rejects(Action action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("unsafe path accepted");
}
int main() {
    try {
        Require(JoinRemotePath("/home/user", "文件 'x';$(id)") == "/home/user/文件 'x';$(id)", "filename changed");
        Require(JoinRemotePath("/", "hello") == "/hello", "root join failed");
        Require(RemoteParent("/home/user/") == "/home", "parent failed");
        Require(RemoteParent("/") == "/", "root parent failed");
        Require(TerminalCdCommand("/home/a'b;$(id)") == "cd -- '/home/a'\"'\"'b;$(id)'\r", "shell quoting failed");
        Rejects([] { TerminalCdCommand("relative"); });
        Rejects([] { ValidateRemotePath("/home\nrm"); });
        Rejects([] { ValidateRemotePath(std::string("/a\0b", 4)); });
        Rejects([] { JoinRemotePath("/home", "../other"); });
        Rejects([] { JoinRemotePath("/home", ".."); });
        Rejects([] { ValidateRemotePath(std::string(5000, 'a')); });
        Rejects([] { ValidateRemotePath(std::string("/\xff", 2)); });
        ValidateLocalFilePath(std::filesystem::path(L"C:/临时/上传 'file.bin"));
        Rejects([] { ValidateLocalFilePath("C:/NUL.txt"); });
        Rejects([] { ValidateLocalFilePath("C:/file:stream"); });
        Rejects([] { ValidateLocalFilePath("C:/trailing."); });
        Rejects([] { ValidateLocalFilePath("relative/file"); });
        std::cout << "file paths and explicit cd PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what(); return 1; }
}
