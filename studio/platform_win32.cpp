// platform.h on Windows: Win32 dialogs and message boxes, CreateProcess, the shell's folders.
#include "platform.h"

#include <windows.h>                    // before the other Win32 headers
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <mutex>

namespace platform {

namespace {
HWND g_owner = nullptr;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path exeDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

fs::path userDataDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    return n ? fs::path(buf) : fs::temp_directory_path();
}

fs::path documentsDir() {
    wchar_t buf[MAX_PATH] = {};
    return SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, buf)) ? fs::path(buf) : userDataDir();
}

FILE* openFile(const fs::path& path, const char* mode) { return _wfopen(path.c_str(), widen(mode).c_str()); }

std::tm localTime(std::time_t t) {
    std::tm tm = {};
    localtime_s(&tm, &t);
    return tm;
}

int run(const std::string& commandLine, const fs::path& workingDir, std::string& out) {
    // Inheritable handles exist only between CreatePipe and the CloseHandle after CreateProcess. Send to Frame and
    // Install run commands from two threads: without this lock one child could inherit the other's pipe, whose
    // reader then never sees end-of-file until that child exits.
    static std::mutex spawn;
    std::unique_lock<std::mutex> spawning(spawn);
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = widen(commandLine);
    const std::wstring cwd = workingDir.wstring();
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    spawning.unlock();
    if (!ok) { CloseHandle(rd); out = "could not start: " + commandLine; return -1; }
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

// Windows' OpenSSH when installed, else Git for Windows' copy, else whatever PATH finds.
std::string sshTool(const char* name) {
    wchar_t found[MAX_PATH] = {};
    const std::wstring exe = widen(name) + L".exe";
    if (SearchPathW(nullptr, exe.c_str(), nullptr, MAX_PATH, found, nullptr)) return "\"" + narrow(found) + "\" ";
    wchar_t win[MAX_PATH] = {};
    GetSystemDirectoryW(win, MAX_PATH);
    std::vector<fs::path> tries = {fs::path(win) / L"OpenSSH" / exe};
    for (const wchar_t* pf : {L"ProgramW6432", L"ProgramFiles", L"LOCALAPPDATA"}) {
        wchar_t d[MAX_PATH] = {};
        if (GetEnvironmentVariableW(pf, d, MAX_PATH)) {
            tries.push_back(fs::path(d) / L"Git" / L"usr" / L"bin" / exe);
            tries.push_back(fs::path(d) / L"Programs" / L"Git" / L"usr" / L"bin" / exe);
        }
    }
    std::error_code ec;
    for (const fs::path& t : tries) if (fs::exists(t, ec)) return "\"" + narrow(t.wstring()) + "\" ";
    return std::string(name) + " ";
}

std::string systemTool(const char* name) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const fs::path p = fs::path(sys) / (widen(name) + L".exe");
    std::error_code ec;
    return fs::exists(p, ec) ? "\"" + narrow(p.wstring()) + "\" " : std::string(name) + " ";
}

void openPath(const std::string& pathOrUrl) {
    ShellExecuteW(nullptr, L"open", widen(pathOrUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool launch(const fs::path& program) {
    std::wstring cmd = L"\"" + program.wstring() + L"\"";
    const std::wstring dir = program.parent_path().wstring();
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(program.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void setDialogOwner(void* nativeWindow) { g_owner = static_cast<HWND>(nativeWindow); }

namespace {
UINT iconFlag(Icon icon) {
    switch (icon) {
        case Icon::Warning: return MB_ICONWARNING;
        case Icon::Error: return MB_ICONERROR;
        case Icon::Question: return MB_ICONQUESTION;
        default: return MB_ICONINFORMATION;
    }
}

// "Meta home (*.apk)\0*.apk\0...All files\0*.*\0\0", as OPENFILENAMEW wants it.
std::wstring filterString(const std::vector<FileFilter>& filters) {
    std::wstring s;
    for (const FileFilter& f : filters) {
        std::wstring patterns;
        for (const std::string& p : f.patterns) patterns += (patterns.empty() ? L"" : L";") + widen(p);
        s += widen(f.description) + L" (" + patterns + L")" + std::wstring(1, L'\0') + patterns + std::wstring(1, L'\0');
    }
    s += L"All files";
    s += L'\0';
    s += L"*.*";
    s += L'\0';
    return s;   // (the string's own terminator ends the list)
}
}  // namespace

void message(const std::string& title, const std::string& text, Icon icon) {
    MessageBoxW(g_owner, widen(text).c_str(), widen(title).c_str(), MB_OK | iconFlag(icon));
}

Answer ask(const std::string& title, const std::string& text, bool withCancel, Icon icon) {
    const int r = MessageBoxW(g_owner, widen(text).c_str(), widen(title).c_str(), (withCancel ? MB_YESNOCANCEL : MB_YESNO) | iconFlag(icon));
    return r == IDYES ? Answer::Yes : r == IDNO ? Answer::No : Answer::Cancel;
}

std::vector<std::string> openFiles(const std::string& title, const std::vector<FileFilter>& filters, bool multiple) {
    std::vector<wchar_t> buf(multiple ? 32768 : 4096, L'\0');
    const std::wstring filter = filterString(filters), wtitle = widen(title);
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.hwndOwner = g_owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (multiple ? OFN_ALLOWMULTISELECT | OFN_EXPLORER : 0);
    if (!GetOpenFileNameW(&ofn)) return {};
    // Multi-select returns "dir\0file1\0file2\0\0"; a single pick returns "full path\0".
    std::vector<std::string> picked;
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    p += first.size() + 1;
    if (!*p) picked.push_back(narrow(first));
    for (; *p; p += std::wcslen(p) + 1) picked.push_back(narrow((fs::path(first) / p).wstring()));
    return picked;
}

std::string saveFile(const std::string& title, const std::string& suggestedName, const std::vector<FileFilter>& filters,
                     const std::string& extension) {
    std::vector<wchar_t> buf(4096, L'\0');
    const std::wstring name = widen(suggestedName), filter = filterString(filters), wtitle = widen(title), ext = widen(extension);
    std::wcsncpy(buf.data(), name.c_str(), buf.size() - 1);
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.hwndOwner = g_owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return narrow(buf.data());
}

}  // namespace platform
