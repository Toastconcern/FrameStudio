#pragma once
// What Frame Home and its Environments panel need from the system, on the Steam Frame (SteamOS) and on a PC running
// SteamVR (Windows, or Linux): where their files are, finding and stopping each other, starting a tool and waiting
// for it, starting again, and a few process settings. Everything else in them is the same everywhere.
//
// Files: the Steam Frame keeps everything in ~/frame-home (environments in envs/). A PC keeps Frame Home's own files
// in %LOCALAPPDATA%\Frame Home (Windows) or ~/frame-home (Linux), and shows the environments saved in Frame Studio's
// My Environments (Documents\Frame Studio\Environments): what you save there is in VR straight away.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <psapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#else
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace homeos {

#ifdef _WIN32
using Pid = DWORD;

inline std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}
inline std::filesystem::path root() {
    if (const char* r = std::getenv("FRAME_HOME_ROOT")) return std::filesystem::u8path(r);   // (tests, a portable copy)
    return knownFolder(FOLDERID_LocalAppData) / L"Frame Home";
}
inline std::filesystem::path envs() {
    if (const char* e = std::getenv("FRAME_HOME_ENVS")) return std::filesystem::u8path(e);
    return knownFolder(FOLDERID_Documents) / L"Frame Studio" / L"Environments";
}
inline std::filesystem::path thumbs() { return root() / L"thumbs"; }   // (not beside the environments: it'd show there)
inline std::string exeName(const char* base) { return std::string(base) + ".exe"; }

inline std::filesystem::path selfPath() {
    wchar_t buf[MAX_PATH * 4] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return n ? std::filesystem::path(buf) : std::filesystem::path();
}
inline bool stderrIsTerminal() { return _isatty(_fileno(stderr)) != 0; }
inline long peakRamMB() {
    PROCESS_MEMORY_COUNTERS pmc = {};
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? static_cast<long>(pmc.PeakWorkingSetSize / (1024 * 1024)) : 0;
}
inline void setEnv(const char* name, const char* value) { _putenv_s(name, value); }
// Background work (thumbnails, unpacking): 15 and up below normal, 19 idle.
inline void lowerPriority(int nice) { SetPriorityClass(GetCurrentProcess(), nice >= 19 ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS); }

// Running copies of `name` other than this one. (Windows doesn't let one process read another's arguments cheaply,
// and the PC has no --system process to tell apart: a copy is a copy.)
inline std::vector<Pid> runningCopies(const char* name) {
    std::vector<Pid> out;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e = {};
    e.dwSize = sizeof e;
    const std::wstring want = std::filesystem::path(exeName(name)).wstring();
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (e.th32ProcessID != GetCurrentProcessId() && _wcsicmp(e.szExeFile, want.c_str()) == 0) out.push_back(e.th32ProcessID);
    CloseHandle(snap);
    return out;
}
inline std::vector<Pid> runningWith(const char*, const std::string&) { return {}; }
inline bool alive(Pid p) {
    const HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, p);
    if (!h) return false;
    const bool running = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return running;
}
// No polite quit for another process's message-less loop on Windows: it's ended (Frame Home keeps nothing unsaved).
inline void stop(Pid p, bool) {
    if (const HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p)) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 5000);
        CloseHandle(h);
    }
}
inline void stopCopies(const char* name) { for (Pid p : runningCopies(name)) stop(p, true); }

inline std::wstring commandLine(const std::vector<std::string>& args) {   // each argument quoted as CreateProcess wants
    std::wstring cl;
    for (const std::string& a : args) {
        const std::wstring w = std::filesystem::u8path(a).wstring();
        if (!cl.empty()) cl += L' ';
        cl += L'"';
        size_t slashes = 0;
        for (wchar_t c : w) {
            if (c == L'\\') { ++slashes; continue; }
            if (c == L'"') cl.append(slashes * 2 + 1, L'\\');
            else cl.append(slashes, L'\\');
            slashes = 0;
            cl += c;
        }
        cl.append(slashes * 2, L'\\');
        cl += L'"';
    }
    return cl;
}
// Starts args[0] with the rest (no window, output discarded); waits when asked. The exit code, or -1.
inline int run(const std::vector<std::string>& args, int nice, bool wait) {
    std::wstring cl = commandLine(args);
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    const DWORD flags = CREATE_NO_WINDOW | (nice >= 19 ? IDLE_PRIORITY_CLASS : nice > 0 ? BELOW_NORMAL_PRIORITY_CLASS : 0);
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) return -1;
    DWORD code = 0;
    if (wait) { WaitForSingleObject(pi.hProcess, INFINITE); GetExitCodeProcess(pi.hProcess, &code); }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
// This program again with the same arguments (settings that build the renderer differently, a lost GPU); this one
// ends. SteamVR knows its home app by its file, so the new one is still the environment.
[[noreturn]] inline void restartSelf() {
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::wstring cl = GetCommandLineW();
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    std::exit(0);
}
#else
using Pid = pid_t;

inline std::filesystem::path root() {
    if (const char* r = std::getenv("FRAME_HOME_ROOT")) return r;   // (tests, a portable copy)
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : "/home/steamos") / "frame-home";
}
inline std::filesystem::path envs() {
    if (const char* e = std::getenv("FRAME_HOME_ENVS")) return e;
    return root() / "envs";
}
inline std::filesystem::path thumbs() { return envs() / ".thumbs"; }
inline std::string exeName(const char* base) { return base; }

inline std::filesystem::path selfPath() {
    char buf[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::filesystem::path(std::string(buf, static_cast<size_t>(n))) : std::filesystem::path();
}
inline bool stderrIsTerminal() { return isatty(2) != 0; }
inline long peakRamMB() { rusage u = {}; getrusage(RUSAGE_SELF, &u); return u.ru_maxrss / 1024; }   // (KB on Linux)
inline void setEnv(const char* name, const char* value) { setenv(name, value, 1); }
inline void lowerPriority(int nice) { setpriority(PRIO_PROCESS, 0, nice); }

// Copies of `name` running without arguments (the environment, the panel: not tools such as --snapshot, --list).
// SteamVR's own bookkeeping (GetApplicationProcessId) loses track of an app once a utility process started from the
// same binary has connected, so /proc it is.
inline std::vector<Pid> runningCopies(const char* name) {
    std::vector<Pid> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string id = e.path().filename().string();
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) continue;
        const Pid pid = static_cast<Pid>(std::stol(id));
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
// Running copies of `name` (other than this process) started with `arg` among their arguments.
inline std::vector<Pid> runningWith(const char* name, const std::string& arg) {
    std::vector<Pid> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
        const std::string id = e.path().filename().string();
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) continue;
        const Pid pid = static_cast<Pid>(std::stol(id));
        if (pid == getpid()) continue;
        std::string comm;
        std::getline(std::ifstream(e.path() / "comm"), comm);
        if (comm != name) continue;
        std::ifstream c(e.path() / "cmdline", std::ios::binary);
        for (std::string a; std::getline(c, a, '\0');) if (a == arg) { out.push_back(pid); break; }
    }
    return out;
}
inline bool alive(Pid p) { return kill(p, 0) == 0; }
inline void stop(Pid p, bool force) { kill(p, force ? SIGKILL : SIGTERM); }
// Asks them to quit, then insists after 5 s.
inline void stopCopies(const char* name) {
    const std::vector<Pid> pids = runningCopies(name);
    for (Pid p : pids) kill(p, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        bool any = false;
        for (Pid p : pids) any |= kill(p, 0) == 0;
        if (!any) return;
        usleep(100000);
    }
    for (Pid p : pids) kill(p, SIGKILL);
}

// Starts args[0] with the rest (output discarded, at `nice`); waits when asked. The exit code, or -1.
// (an argv, not a shell line: an environment's name can hold any character)
inline int run(const std::vector<std::string>& args, int nice, bool wait) {
    std::vector<std::string> a = args;
    std::vector<char*> argv;
    for (std::string& s : a) argv.push_back(s.data());
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        if (nice > 0) setpriority(PRIO_PROCESS, 0, nice);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 1); dup2(null, 2); }
        execv(argv[0], argv.data());
        _exit(127);
    }
    if (pid < 0) return -1;
    if (!wait) return 0;
    int st = 0;
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
// This program again with the same arguments, in this process (SteamVR knows it by its path: still the environment).
[[noreturn]] inline void restartSelf() {
    const std::string self = selfPath().string();
    std::ifstream cl("/proc/self/cmdline", std::ios::binary);
    std::vector<std::string> args;
    for (std::string a; std::getline(cl, a, '\0');) args.push_back(a);
    if (args.empty()) args.push_back(self);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    execv(self.c_str(), argv.data());
    std::exit(1);
}
#endif

inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Is SteamVR running (its server)? Asked before connecting as a utility: on Windows, connecting starts SteamVR.
inline bool steamVrRunning() {
#ifdef _WIN32
    bool found = false;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e = {};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) found = _wcsicmp(e.szExeFile, L"vrserver.exe") == 0;
    CloseHandle(snap);
    return found;
#else
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc", ec)) {
        std::string comm;
        std::getline(std::ifstream(e.path() / "comm"), comm);
        if (comm == "vrserver") return true;
    }
    return false;
#endif
}

// Files by path, whatever the characters in it (Windows: the wide API; a user name like "José" is no problem).
inline FILE* openFile(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    const std::wstring m(mode, mode + std::strlen(mode));
    return _wfopen(p.wstring().c_str(), m.c_str());
#else
    return std::fopen(p.c_str(), mode);
#endif
}
inline bool reopenStderr(const std::filesystem::path& p, const char* mode) {
#ifdef _WIN32
    const std::wstring m(mode, mode + std::strlen(mode));
    return _wfreopen(p.wstring().c_str(), m.c_str(), stderr) != nullptr;
#else
    return std::freopen(p.c_str(), mode, stderr) != nullptr;
#endif
}

// The memory this process uses now, MB.
inline double ramMB() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc = {};
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize / 1048576.0 : 0.0;
#else
    long pages = 0, rssPages = 0;
    if (FILE* sm = std::fopen("/proc/self/statm", "r")) { if (std::fscanf(sm, "%ld %ld", &pages, &rssPages) != 2) rssPages = 0; std::fclose(sm); }
    return rssPages * (sysconf(_SC_PAGESIZE) / 1048576.0);
#endif
}

}  // namespace homeos
