#pragma once
// Running copies of a Frame program, for the installers. SteamVR's own bookkeeping (GetApplicationProcessId) loses
// track of an app once a utility process started from the same binary has connected, so look at /proc instead.
#include <signal.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Processes named `name` running without arguments: the environment / the panel, not tools (--snapshot, --list).
inline std::vector<pid_t> runningCopies(const char* name) {
    std::vector<pid_t> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string id = e.path().filename().string();
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) continue;
        const pid_t pid = static_cast<pid_t>(std::stol(id));
        if (pid == getpid()) continue;
        std::string comm;
        std::getline(std::ifstream(e.path() / "comm"), comm);
        if (comm != name) continue;
        std::ifstream c(e.path() / "cmdline", std::ios::binary);
        const std::string cmd((std::istreambuf_iterator<char>(c)), std::istreambuf_iterator<char>());
        size_t args = 0;
        for (char ch : cmd) args += ch == '\0';
        if (args <= 1) out.push_back(pid);
    }
    return out;
}

// Asks them to quit, then insists after 5 s.
// Running copies of `name` (other than this process) started with `arg` among their arguments.
inline std::vector<pid_t> runningWith(const char* name, const std::string& arg) {
    std::vector<pid_t> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string id = e.path().filename().string();
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) continue;
        const pid_t pid = static_cast<pid_t>(std::stol(id));
        if (pid == getpid()) continue;
        std::string comm;
        std::getline(std::ifstream(e.path() / "comm"), comm);
        if (comm != name) continue;
        std::ifstream c(e.path() / "cmdline", std::ios::binary);
        for (std::string a; std::getline(c, a, '\0');) if (a == arg) { out.push_back(pid); break; }
    }
    return out;
}

inline void stopCopies(const char* name) {
    const std::vector<pid_t> pids = runningCopies(name);
    for (pid_t p : pids) kill(p, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        bool alive = false;
        for (pid_t p : pids) alive |= kill(p, 0) == 0;
        if (!alive) return;
        usleep(100000);
    }
    for (pid_t p : pids) kill(p, SIGKILL);
}
