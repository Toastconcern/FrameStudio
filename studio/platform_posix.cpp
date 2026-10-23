// platform.h on Linux and macOS: dialogs through tinyfiledialogs (zenity / kdialog on Linux, the system's own panels
// on macOS), programs through /bin/sh, folders after the XDG base directories (Linux) or ~/Library (macOS).
#include "platform.h"

#include <tinyfiledialogs.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern char** environ;

namespace platform {

namespace {

fs::path home() {
    const char* h = std::getenv("HOME");
    return h && *h ? fs::path(h) : fs::temp_directory_path();
}

// The text a dialog shows: tinyfiledialogs refuses quotes (it hands the text to a shell), so they become the
// typographic ones ("Couldn't" reads the same as "Couldn’t").
std::string dialogText(const std::string& s) {
    std::string out;
    for (char c : s) out += c == '\'' || c == '`' ? "’" : c == '"' ? "”" : std::string(1, c);
    return out;
}

// One argument for /bin/sh: in single quotes, its own single quotes closed, escaped and reopened.
std::string shellQuote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

const char* iconName(Icon icon) {
    switch (icon) {
        case Icon::Warning: return "warning";
        case Icon::Error: return "error";
        case Icon::Question: return "question";
        default: return "info";
    }
}

// tinyfiledialogs takes one filter: every pattern of every line, described by the first line.
struct OneFilter {
    std::vector<std::string> patterns;
    std::vector<const char*> pointers;
    std::string description;
    explicit OneFilter(const std::vector<FileFilter>& filters) {
        for (const FileFilter& f : filters) {
            if (description.empty()) description = f.description;
            patterns.insert(patterns.end(), f.patterns.begin(), f.patterns.end());
        }
        for (const std::string& p : patterns) pointers.push_back(p.c_str());
    }
};

}  // namespace

// wchar_t is UTF-32 here: decode and encode UTF-8 directly.
std::wstring widen(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int extra = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        uint32_t cp = extra == 0 ? c : extra == 1 ? c & 0x1F : extra == 2 ? c & 0x0F : c & 0x07;
        if (extra > 0 && i + extra >= s.size()) { w += L'�'; break; }   // a sequence cut short
        for (int k = 1; k <= extra; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        w += static_cast<wchar_t>(cp);
        i += 1 + extra;
    }
    return w;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        const uint32_t cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) s += static_cast<char>(cp);
        else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return s;
}

fs::path exeDir() {
    std::error_code ec;
#ifdef __APPLE__
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return fs::canonical(buf, ec).parent_path();
    return fs::current_path(ec);
#else
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path(ec) : self.parent_path();
#endif
}

fs::path userDataDir() {
#ifdef __APPLE__
    return home() / "Library" / "Application Support";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    return xdg && *xdg ? fs::path(xdg) : home() / ".local" / "share";
#endif
}

fs::path documentsDir() {
    static const fs::path found = [] {
        std::error_code ec;
#ifndef __APPLE__
        // xdg-user-dirs' answer (localized folder names), when it's set up
        std::string out;
        if (run("xdg-user-dir DOCUMENTS", {}, out) == 0) {
            while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
            if (!out.empty() && fs::is_directory(out, ec) && fs::path(out) != home()) return fs::path(out);
        }
#endif
        const fs::path docs = home() / "Documents";
        return fs::is_directory(docs, ec) ? docs : home();
    }();
    return found;
}

FILE* openFile(const fs::path& path, const char* mode) { return std::fopen(path.c_str(), mode); }

std::tm localTime(std::time_t t) {
    std::tm tm = {};
    localtime_r(&t, &tm);
    return tm;
}

int run(const std::string& commandLine, const fs::path& workingDir, std::string& out) {
    // Both ends close on exec: a command started meanwhile by another thread (Send to Frame and Install run at once)
    // must not inherit this pipe, or its reader never sees the end. The child's own copies (dup2) stay open.
    int pipeFds[2];
    {
        static std::mutex creating;   // (no pipe2 on macOS: made and marked as one step)
        std::lock_guard<std::mutex> lock(creating);
        if (::pipe(pipeFds) != 0) return -1;
        ::fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(pipeFds[1], F_SETFD, FD_CLOEXEC);
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 2);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    posix_spawn_file_actions_addclose(&actions, pipeFds[1]);
    // (the working directory through the shell: posix_spawn_file_actions_addchdir isn't everywhere yet)
    const std::string line = workingDir.empty() ? commandLine : "cd " + shellQuote(workingDir.string()) + " && " + commandLine;
    const char* argv[] = {"/bin/sh", "-c", line.c_str(), nullptr};
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, "/bin/sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipeFds[1]);
    if (spawned != 0) { ::close(pipeFds[0]); out = "could not start: " + commandLine; return -1; }
    char buf[4096];
    for (ssize_t n; (n = ::read(pipeFds[0], buf, sizeof buf)) > 0;) out.append(buf, static_cast<size_t>(n));
    ::close(pipeFds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string sshTool(const char* name) { return std::string(name) + " "; }
std::string systemTool(const char* name) { return std::string(name) + " "; }

bool launch(const fs::path& program) {
#ifdef __APPLE__
    const std::string cmd = "open -n " + shellQuote(program.string()) + " >/dev/null 2>&1";
#else   // (its own output away from run's pipe, or run would wait for it to end)
    const std::string cmd = "cd " + shellQuote(program.parent_path().string()) + " && nohup " + shellQuote(program.string()) + " >/dev/null 2>&1 &";
#endif
    std::string out;
    return run(cmd, {}, out) == 0;
}

void openPath(const std::string& pathOrUrl) {
#ifdef __APPLE__
    const std::string cmd = "open " + shellQuote(pathOrUrl) + " >/dev/null 2>&1 &";
#else
    const std::string cmd = "xdg-open " + shellQuote(pathOrUrl) + " >/dev/null 2>&1 &";
#endif
    std::string ignored;
    run(cmd, {}, ignored);
}

void setDialogOwner(void*) {}

void message(const std::string& title, const std::string& text, Icon icon) {
    tinyfd_messageBox(dialogText(title).c_str(), dialogText(text).c_str(), "ok", iconName(icon), 1);
}

Answer ask(const std::string& title, const std::string& text, bool withCancel, Icon icon) {
    const int r = tinyfd_messageBox(dialogText(title).c_str(), dialogText(text).c_str(), withCancel ? "yesnocancel" : "yesno", iconName(icon), 1);
    // tinyfiledialogs: 1 yes; yesnocancel: 2 no, 0 cancel; yesno: 0 no
    return r == 1 ? Answer::Yes : withCancel && r == 0 ? Answer::Cancel : Answer::No;
}

std::vector<std::string> openFiles(const std::string& title, const std::vector<FileFilter>& filters, bool multiple) {
    const OneFilter f(filters);
    const char* picked = tinyfd_openFileDialog(dialogText(title).c_str(), nullptr, static_cast<int>(f.pointers.size()),
                                               f.pointers.empty() ? nullptr : f.pointers.data(),
                                               f.description.empty() ? nullptr : f.description.c_str(), multiple ? 1 : 0);
    std::vector<std::string> out;
    if (!picked) return out;
    // several files come back joined by '|'
    std::string all = picked;
    for (size_t at = 0; at <= all.size();) {
        const size_t bar = all.find('|', at);
        const std::string one = all.substr(at, bar == std::string::npos ? std::string::npos : bar - at);
        if (!one.empty()) out.push_back(one);
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

std::string saveFile(const std::string& title, const std::string& suggestedName, const std::vector<FileFilter>& filters,
                     const std::string& extension) {
    const OneFilter f(filters);
    const char* picked = tinyfd_saveFileDialog(dialogText(title).c_str(), suggestedName.c_str(), static_cast<int>(f.pointers.size()),
                                               f.pointers.empty() ? nullptr : f.pointers.data(),
                                               f.description.empty() ? nullptr : f.description.c_str());
    if (!picked) return {};
    std::string path = picked;
    if (!extension.empty() && fs::path(path).extension().empty()) path += "." + extension;
    return path;
}

}  // namespace platform
