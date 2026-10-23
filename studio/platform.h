#pragma once
// What Frame Studio needs from the operating system, behind one interface: platform_win32.cpp (Windows) and
// platform_posix.cpp (Linux, macOS). Text and paths cross it as UTF-8 std::string (or fs::path); the rest of Studio
// never includes a system header.
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace platform {

namespace fs = std::filesystem;

// ── Text ──
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

// ── Places ──
fs::path exeDir();                       // the folder Frame Studio runs from (the Frame payload ships beside it)
fs::path userDataDir();                  // %APPDATA% / $XDG_DATA_HOME (~/.local/share) / ~/Library/Application Support
fs::path documentsDir();                 // Documents (the user's own; ~ when there's none)
FILE* openFile(const fs::path& path, const char* mode);   // fopen with a Unicode path everywhere
std::tm localTime(std::time_t t);        // (thread-safe localtime)

// ── Other programs ──
// Runs a command line (no console window); stdout and stderr land in `out`. Returns its exit code (-1: not started).
// The line is in the platform's shell syntax as far as quoting goes: "double quotes" around arguments work on both.
int run(const std::string& commandLine, const fs::path& workingDir, std::string& out);
// ssh / scp, ready to prefix arguments: quoted when it's a path, with a trailing space.
std::string sshTool(const char* name);
// The system's own curl / tar (Windows 10 and later ship both in System32; Git's tar there can't open a .zip), ready
// to prefix arguments like sshTool.
std::string systemTool(const char* name);
// Opens a folder in the file browser, or a file / URL in its usual program.
void openPath(const std::string& pathOrUrl);
// Starts a program (or, on macOS, a .app) on its own: not waited for, it outlives Frame Studio. False: not started.
bool launch(const fs::path& program);

// ── Dialogs ── (modal; they block until answered)
// The window dialogs belong to (Windows: its HWND), so they come up over it. Null: none.
void setDialogOwner(void* nativeWindow);
enum class Icon { Info, Warning, Error, Question };
void message(const std::string& title, const std::string& text, Icon icon = Icon::Info);
enum class Answer { Yes, No, Cancel };
Answer ask(const std::string& title, const std::string& text, bool withCancel, Icon icon = Icon::Question);

// One line of a file dialog's type list: "Meta home" with {"*.apk"}. "All files" is always added after them.
struct FileFilter {
    std::string description;
    std::vector<std::string> patterns;
};
// The files picked (empty: cancelled). `multiple`: several may be picked at once.
std::vector<std::string> openFiles(const std::string& title, const std::vector<FileFilter>& filters, bool multiple);
// Where to save (empty: cancelled). `suggestedName` is filled in; `extension` ("fenv", no dot) is added to a name
// typed without one. Asks before replacing a file that exists.
std::string saveFile(const std::string& title, const std::string& suggestedName, const std::vector<FileFilter>& filters,
                     const std::string& extension);

}  // namespace platform
